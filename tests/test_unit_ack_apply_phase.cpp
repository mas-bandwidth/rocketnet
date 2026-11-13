/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The deferred ack apply (sdk/rocketnet_ack.h, rocketnet_server_acks_t;
// proofs/tla/RNAckApplyPhase.tla): the server's update queues each received
// block on its client, the game's jobs apply each client between
// update_receive and update_finish, and the result is the inline apply's.
//
// The differential (tests/ack_apply_differential.h) runs one trace three
// ways, inline, deferred whole and deferred by a schedule, and compares the
// batch, the counters and every endpoint after every update. Here the
// schedules run on the test's one thread: every client in a shuffled order,
// a client asked twice, a few clients and the rest left to the close. The
// same trace on real threads is the functional case
// test_ack_apply_jobs_match_inline (tests/functional/ack_apply_threads.cpp).
//
// The transport cases drive a server over the in-memory loopback transport,
// time from 1000.0, no socket: the split update against a twin server whose
// create kept the inline apply, and the split calls' misuse refusals.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "portable_crt.h"

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ACK_APPLY_CHECK( condition ) rocketnet_unit_check( condition )
#include "ack_apply_differential.h"

// every client once, in the shuffled order: the jobs taking all of them
static void ack_schedule_all( struct rocketnet_server_acks_t * acks, const int * order, int count, uint64_t * rng )
{
    (void) rng;
    for ( int i = 0; i < count; i++ )
    {
        (void) rocketnet_server_acks_apply_client( acks, order[i] );
    }
}

// every client, some twice: the second call finds the claim taken and applies
// nothing
static void ack_schedule_twice( struct rocketnet_server_acks_t * acks, const int * order, int count, uint64_t * rng )
{
    for ( int i = 0; i < count; i++ )
    {
        (void) rocketnet_server_acks_apply_client( acks, order[i] );
        if ( ack_apply_below( rng, 2 ) )
        {
            rocketnet_unit_check( rocketnet_server_acks_apply_client( acks, order[i] ) == 0 );
        }
    }
}

// a few clients; the close applies the rest on main
static void ack_schedule_some( struct rocketnet_server_acks_t * acks, const int * order, int count, uint64_t * rng )
{
    const int taken = (int) ack_apply_below( rng, (uint64_t) count + 1 );
    for ( int i = 0; i < taken; i++ )
    {
        (void) rocketnet_server_acks_apply_client( acks, order[i] );
    }
}

static void ack_apply_check_reached( const struct ack_apply_stats_t * stats )
{
    rocketnet_unit_check( stats->blocks > 1000 );
    rocketnet_unit_check( stats->early_gathers > 0 );
    rocketnet_unit_check( stats->settles_send > 0 );
    rocketnet_unit_check( stats->settles_reset > 0 );
    rocketnet_unit_check( stats->memo_repeats > 0 );
    rocketnet_unit_check( stats->long_blocks > 0 );
    rocketnet_unit_check( stats->acked > 1000 );
}

static void test_ack_apply_deferred_matches_inline()
{
    const uint64_t seeds[] = { 0xACC0A991ull, 0x5EED0002ull, 0xBADC0DE3ull };
    for ( uint64_t seed : seeds )
    {
        struct ack_apply_stats_t stats;
        memset( &stats, 0, sizeof( stats ) );
        ack_apply_differential( seed, 200, ack_schedule_all, &stats );
        ack_apply_check_reached( &stats );
    }
}

REGISTER_UNIT_TEST( test_ack_apply_deferred_matches_inline );

static void test_ack_apply_any_schedule_matches_inline()
{
    struct ack_apply_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    ack_apply_differential( 0x7A1CE5ull, 200, ack_schedule_twice, &stats );
    ack_apply_check_reached( &stats );

    memset( &stats, 0, sizeof( stats ) );
    ack_apply_differential( 0x50A1E5ull, 200, ack_schedule_some, &stats );
    ack_apply_check_reached( &stats );
}

REGISTER_UNIT_TEST( test_ack_apply_any_schedule_matches_inline );

// the phases: queued blocks change nothing until applied; a client's apply
// is taken once; the close leaves nothing queued; an inline acks ignores the
// deferral calls
static void test_ack_apply_phases_and_claims()
{
    struct rocketnet_server_acks_t * acks = rocketnet_server_acks_create( NULL, 2 );
    struct rocketnet_server_acks_t * plain = rocketnet_server_acks_create( NULL, 2 );
    rocketnet_unit_check( acks && plain );
    rocketnet_unit_check( rocketnet_server_acks_defer( acks ) );
    rocketnet_unit_check( acks->deferred == 1 && plain->deferred == 0 );
    rocketnet_unit_check( rocketnet_server_acks_defer_allocation_bytes( 0 ) == 0 );
    rocketnet_unit_check( rocketnet_server_acks_defer_allocation_bytes( 2 ) > 0 );

    for ( uint64_t s = 1; s <= 8; s++ )
    {
        rocketnet_ack_endpoint_record_sent_classified( &acks->endpoints[1], s, 100, 7000 + s, 1, 1000.0 );
        rocketnet_ack_endpoint_record_sent_classified( &plain->endpoints[1], s, 100, 7000 + s, 1, 1000.0 );
    }

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = 8;
    block.first_range_length = 8;

    uint64_t ids[2][2 * ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    uint64_t unknown[2] = { 0, 0 };
    uint64_t full[2] = { 0, 0 };

    rocketnet_server_acks_begin_receive( acks, 1000.5 );
    rocketnet_server_acks_begin_receive( plain, 1000.5 );
    rocketnet_unit_check( acks->phase == ROCKETNET_SERVER_ACKS_RECEIVING );
    rocketnet_unit_check( plain->phase == ROCKETNET_SERVER_ACKS_CLOSED );

    rocketnet_server_acks_receive_block( acks, 1, &block, 1000.5, 0xC1, ids[0], &unknown[0], &full[0] );
    rocketnet_server_acks_receive_block( plain, 1, &block, 1000.5, 0xC1, ids[1], &unknown[1], &full[1] );

    // queued: nothing acked yet, nothing in the batch; the inline acks applied
    rocketnet_unit_check( acks->num_pending == 1 && acks->num_acked == 0 );
    rocketnet_unit_check( acks->endpoints[1].largest_acked_sequence == 0 );
    rocketnet_unit_check( plain->num_acked == 8 && plain->endpoints[1].largest_acked_sequence == 8 );

    // the empty block queues nothing
    struct rocketnet_ack_block_t empty;
    memset( &empty, 0, sizeof( empty ) );
    rocketnet_server_acks_receive_block( acks, 0, &empty, 1000.5, 0xC0, ids[0], &unknown[0], &full[0] );
    rocketnet_unit_check( acks->num_pending == 1 );

    rocketnet_server_acks_open( acks );
    rocketnet_server_acks_open( plain );
    rocketnet_unit_check( acks->phase == ROCKETNET_SERVER_ACKS_OPEN );

    rocketnet_unit_check( rocketnet_server_acks_apply_client( acks, 0 ) == 0 );     // nothing queued
    rocketnet_unit_check( rocketnet_server_acks_apply_client( acks, 1 ) == 1 );
    rocketnet_unit_check( rocketnet_server_acks_apply_client( acks, 1 ) == 0 );     // taken already
    rocketnet_unit_check( rocketnet_server_acks_apply_client( acks, 2 ) == 0 );     // out of range
    rocketnet_unit_check( rocketnet_server_acks_apply_client( acks, -1 ) == 0 );
    rocketnet_unit_check( rocketnet_server_acks_apply_client( plain, 1 ) == 0 );    // inline: nothing to take
    rocketnet_unit_check( acks->endpoints[1].largest_acked_sequence == 8 );
    rocketnet_unit_check( acks->num_acked == 0 );                                     // published only at the close

    rocketnet_server_acks_close( acks, ids[0], &unknown[0], &full[0] );
    rocketnet_server_acks_close( plain, ids[1], &unknown[1], &full[1] );
    rocketnet_unit_check( acks->phase == ROCKETNET_SERVER_ACKS_CLOSED && acks->num_pending == 0 );
    rocketnet_unit_check( acks->num_acked == 8 );
    for ( int i = 0; i < 8; i++ )
    {
        rocketnet_unit_check( acks->acked_packet_ids[i] == plain->acked_packet_ids[i] );
        rocketnet_unit_check( acks->acked_client_index[i] == 1 );
        rocketnet_unit_check( ids[0][i] == 0xC1 && ids[1][i] == 0xC1 );
    }
    rocketnet_unit_check( unknown[0] == unknown[1] && full[0] == full[1] );
    rocketnet_unit_check( rocketnet_ack_endpoint_state_equal( &acks->endpoints[1], &plain->endpoints[1] ) );

    // a block received outside the receive phase applies inline even when deferred
    rocketnet_ack_endpoint_record_sent_classified( &acks->endpoints[0], 1, 100, 9001, 1, 1001.0 );
    block.largest = 1;
    block.first_range_length = 1;
    acks->num_acked = 0;
    rocketnet_server_acks_receive_block( acks, 0, &block, 1001.0, 0xC0, ids[0], &unknown[0], &full[0] );
    rocketnet_unit_check( acks->num_acked == 1 && acks->acked_packet_ids[0] == 9001 && acks->num_pending == 0 );

    rocketnet_server_acks_destroy( acks );
    rocketnet_server_acks_destroy( plain );
}

REGISTER_UNIT_TEST( test_ack_apply_phases_and_claims );

// ---------------------------------------------------------------
//             the transport: the split update, and its misuse
// ---------------------------------------------------------------

#define ACK_PHASE_CLIENTS 3

static double ack_phase_clock = 1000.0;
static double ack_phase_time_callback( void * ) { return ack_phase_clock; }

struct ack_phase_world_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * clients[ACK_PHASE_CLIENTS];
};

static bool ack_phase_world_setup( struct ack_phase_world_t * world, bool inline_acks, uint64_t protocol_id )
{
    memset( world, 0, sizeof( *world ) );

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = ACK_PHASE_CLIENTS;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    rocketnet_server_transport_set_create_inline_acks_for_test( inline_acks );
    world->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    rocketnet_server_transport_set_create_inline_acks_for_test( false );
    rocketnet_unit_check( world->server != NULL );
    if ( !world->server )
    {
        return false;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( world->server ) );
    const char * addresses[] = { server_address };
    static const uint64_t client_ids[ACK_PHASE_CLIENTS] = { 0x9e3779b97f4a7c15ULL, 0x9e3779b97f4a7c16ULL, 0x9e3779b97f4a7c17ULL };

    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                                        client_ids[c], 0, 1ULL << 40, 60, NULL,
                                                                        token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

        struct rocketnet_client_transport_config_t client_config;
        rocketnet_client_transport_default_config_current( &client_config );
        snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = 1;
        client_config.time_callback = ack_phase_time_callback;

        world->clients[c] = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), ack_phase_clock );
        rocketnet_unit_check( world->clients[c] != NULL );
        if ( !world->clients[c] )
        {
            return false;
        }
    }
    return true;
}

static void ack_phase_world_teardown( struct ack_phase_world_t * world )
{
    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        rn_test_client_transport_stop_and_destroy( world->clients[c] );
    }
    rn_test_server_transport_stop_and_destroy( world->server );
}

// a server update, whole or split; split takes the clients in reverse
static int ack_phase_server_update( struct rocketnet_server_transport_t * server, bool split, double time )
{
    if ( !split )
    {
        rocketnet_server_transport_update( server, time );
        return 0;
    }
    rocketnet_server_transport_update_receive( server, time );
    int applied = 0;
    for ( int c = ACK_PHASE_CLIENTS - 1; c >= 0; c-- )
    {
        applied += rocketnet_server_transport_update_client_acks( server, c );
    }
    rocketnet_server_transport_update_finish( server );
    return applied;
}

// one frame of a world: the clients' updates, the server's, each side's few
// payloads, the flushes. Returns the acked ids the server surfaced, and
// writes their client indexes
static int ack_phase_frame( struct ack_phase_world_t * world, bool split, int frame, int * client_counts, int * applied )
{
    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        rocketnet_client_transport_update( world->clients[c], ack_phase_clock );
    }
    *applied += ack_phase_server_update( world->server, split, ack_phase_clock );

    const int * client_index = NULL;
    const uint64_t * ids = NULL;
    const int acked = rocketnet_server_transport_acked_packets( world->server, &client_index, &ids );
    for ( int i = 0; i < acked; i++ )
    {
        rocketnet_unit_check( client_index[i] >= 0 && client_index[i] < ACK_PHASE_CLIENTS );
        client_counts[client_index[i]]++;
    }

    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        if ( !rocketnet_server_transport_is_client_connected( world->server, c ) )
        {
            continue;
        }
        const int sends = 1 + ( frame + c ) % 3;
        for ( int s = 0; s < sends; s++ )
        {
            struct rocketnet_packet_context_t context;
            uint8_t * packet = rocketnet_server_transport_start_packet( world->server, c, 32, &context );
            if ( packet )
            {
                memset( context.frame + context.payload_index, 0x5A, 32 );
                rocketnet_unit_check( rocketnet_server_transport_finish_packet( world->server, &context, 32 ) > 0 );
            }
        }
    }
    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        if ( rocketnet_client_transport_state( world->clients[c] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            continue;
        }
        struct rocketnet_packet_context_t context;
        uint8_t * packet = rocketnet_client_transport_start_packet( world->clients[c], 32, &context );
        if ( packet )
        {
            memset( context.frame + context.payload_index, 0xA5, 32 );
            rocketnet_unit_check( rocketnet_client_transport_finish_packet( world->clients[c], &context, 32 ) > 0 );
        }
    }

    rocketnet_server_transport_send_packets( world->server );
    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        rocketnet_client_transport_send_packets( world->clients[c] );
    }
    return acked;
}

// the split update with the clients taken in reverse, against a twin whose
// create kept the inline apply, run one after the other (one server at a
// time fits the case's memory): the same acked counts per client and frame,
// the same ack counters, the same per-client rtt

#define ACK_PHASE_FRAMES 300

struct ack_phase_run_t
{
    int counts[ACK_PHASE_FRAMES][ACK_PHASE_CLIENTS];
    int applied;
    int total;
    uint64_t unknown;
    uint64_t batch_full;
    double rtt[ACK_PHASE_CLIENTS];
    int connected[ACK_PHASE_CLIENTS];
};

static void ack_phase_run( bool split, uint64_t protocol_id, struct ack_phase_run_t * run )
{
    memset( run, 0, sizeof( *run ) );
    ack_phase_clock = 1000.0;

    struct ack_phase_world_t world;
    rocketnet_unit_check( ack_phase_world_setup( &world, !split, protocol_id ) );
    if ( !world.server )
    {
        return;
    }

    for ( int frame = 0; frame < ACK_PHASE_FRAMES; frame++ )
    {
        ack_phase_clock += 0.01;
        run->total += ack_phase_frame( &world, split, frame, run->counts[frame], &run->applied );
    }

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( world.server );
    run->unknown = counters->values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE];
    run->batch_full = counters->values[ROCKETNET_COUNTER_ACKED_BATCH_FULL];
    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        run->connected[c] = rocketnet_server_transport_is_client_connected( world.server, c );
        run->rtt[c] = rocketnet_server_transport_client_rtt( world.server, c );
    }

    ack_phase_world_teardown( &world );
}

static void test_ack_apply_split_update_matches_inline_server()
{
    rocketnet_quiet( 1 );

    struct ack_phase_run_t * runs = (struct ack_phase_run_t *) rocketnet_malloc( NULL, 2 * sizeof( struct ack_phase_run_t ) );
    rocketnet_unit_check( runs != NULL );
    if ( !runs )
    {
        return;
    }

    ack_phase_run( false, 0x61636b696e6c6e65ULL, &runs[0] );
    ack_phase_run( true, 0x61636b73706c6974ULL, &runs[1] );

    for ( int frame = 0; frame < ACK_PHASE_FRAMES; frame++ )
    {
        for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
        {
            rocketnet_unit_check( runs[0].counts[frame][c] == runs[1].counts[frame][c] );
        }
    }
    rocketnet_unit_check( runs[0].total == runs[1].total );
    rocketnet_unit_check( runs[0].total > ACK_PHASE_FRAMES );
    rocketnet_unit_check( runs[0].applied == 0 );
    rocketnet_unit_check( runs[1].applied > ACK_PHASE_FRAMES );
    rocketnet_unit_check( runs[0].unknown == runs[1].unknown );
    rocketnet_unit_check( runs[0].batch_full == runs[1].batch_full );
    for ( int c = 0; c < ACK_PHASE_CLIENTS; c++ )
    {
        rocketnet_unit_check( runs[0].connected[c] && runs[1].connected[c] );
        rocketnet_unit_check( runs[0].rtt[c] == runs[1].rtt[c] );
    }

    rocketnet_free( NULL, runs );
}

REGISTER_UNIT_TEST( test_ack_apply_split_update_matches_inline_server );

// the misuse below asserts in a debug build; the case reads the counts
static void ack_phase_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

// update_client_acks outside update_receive .. update_finish, or with a bad
// index, is misuse: counted, and it does nothing. A receive with the last
// one open finishes it first, counted. A finish with nothing open does nothing.
static void test_ack_apply_split_update_misuse_is_counted()
{
    rocketnet_quiet( 1 );
    ack_phase_clock = 1000.0;

    struct ack_phase_world_t world;
    rocketnet_unit_check( ack_phase_world_setup( &world, false, 0x61636b6d69737573ULL ) );

    rocketnet_assert_function( ack_phase_quiet_assert );

    uint64_t before = rocketnet_api_misuse_count();
    rocketnet_unit_check( rocketnet_server_transport_update_client_acks( world.server, 0 ) == 0 );
    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 1 );

    rocketnet_server_transport_update_receive( world.server, 1000.01 );
    before = rocketnet_api_misuse_count();
    rocketnet_unit_check( rocketnet_server_transport_update_client_acks( world.server, ACK_PHASE_CLIENTS ) == 0 );
    rocketnet_unit_check( rocketnet_server_transport_update_client_acks( world.server, -1 ) == 0 );
    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 2 );

    before = rocketnet_api_misuse_count();
    rocketnet_server_transport_update_receive( world.server, 1000.02 );         // the last receive is still open
    rocketnet_unit_check( rocketnet_api_misuse_count() == before + 1 );
    rocketnet_server_transport_update_finish( world.server );

    before = rocketnet_api_misuse_count();
    rocketnet_server_transport_update_finish( world.server );                  // nothing open: nothing
    rocketnet_unit_check( rocketnet_api_misuse_count() == before );

    rocketnet_assert_function( NULL );

    ack_phase_world_teardown( &world );
}

REGISTER_UNIT_TEST( test_ack_apply_split_update_misuse_is_counted );

/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The ack phase (SERVER-UPDATE-ACK.md P3, proofs/tla/RNAckPhase.tla): the
// split update, rocketnet_server_update_begin, rocketnet_server_update_acks
// for any clients from the game's job threads, rocketnet_server_update_end,
// leaves what one rocketnet_server_update leaves.
//
//   - the differential: two worlds in lockstep over seeds, the same clients,
//     traffic, losses, disconnects and reconnects. World A updates with the
//     serial reference (rocketnet_server_acks_test_direct: each ack block
//     applied in the receive loop, as before the ack phase existed). World B
//     makes the split update, its ack calls from several threads, each
//     frame's clients shuffled and dealt to the threads afresh, some clients
//     left for update_end. After every update the two must hold the same
//     acked batch (ids, clients and connection tags, in order), every
//     transport and message counter, every ack endpoint (state_equal), the
//     message layer's ack timing state and receipts; and every message the
//     game receives on either side is the same. Some seeds lower the
//     entries an update defers, so it spills in the receive loop.
//   - the refusals, driven step by step: a fill call, a second main-thread
//     operation and the one-call update inside an open update; an ack call
//     and update_end outside one; two threads acking one client in one
//     phase. Each refused, counted (FILL_PHASE_OVERLAP, FILL_TWO_FILLERS) and
//     asserted in a debug build.
//
// No socket, no real time: loopback hubs and the test's own clock. The ack
// threads are joined before update_end, which is the contract's handoff.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "token_string.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_ack.h"
#include "rocketnet_counters.h"
#include "rocketnet_fill_phase.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

#define check rocketnet_unit_check

#define ACK_PHASE_PROTOCOL 0x41434B50F1110003ULL
#define ACK_PHASE_MAX_CLIENTS 16
#define ACK_PHASE_THREADS_MAX 8

static double g_ack_phase_now = 1000.0;
static double ack_phase_clock( void * ) { return g_ack_phase_now; }

static int g_ack_phase_asserts = 0;
static void ack_phase_assert_hook( const char *, const char *, const char *, int )
{
    g_ack_phase_asserts++;
}

static int ack_phase_expected_asserts( int fired_in_debug )
{
#if ROCKETNET_ASSERTS
    return fired_in_debug;
#else // #if ROCKETNET_ASSERTS
    (void) fired_in_debug;
    return 0;
#endif // #if ROCKETNET_ASSERTS
}

static uint64_t ack_phase_random( uint64_t * state )
{
    uint64_t z = ( *state += 0x9E3779B97F4A7C15ULL );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBULL;
    return z ^ ( z >> 31 );
}

static int ack_phase_below( uint64_t * state, int n )
{
    return (int) ( ack_phase_random( state ) % (uint64_t) n );
}

struct ack_phase_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_loopback_hub_t * hub;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[ACK_PHASE_MAX_CLIENTS];
    int num_clients;
    uint64_t messages_hash;             // every message the game took, both sides, in order
    uint64_t messages;
};

static void ack_phase_hash( uint64_t * hash, uint64_t value )
{
    for ( int i = 0; i < 8; i++ )
    {
        *hash ^= (uint8_t) ( value >> ( 8 * i ) );
        *hash *= 0x100000001b3ULL;
    }
}

static void ack_phase_world_create( ack_phase_world_t * world, int num_clients )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->messages_hash = 0xcbf29ce484222325ULL;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );

    world->hub = rocketnet_loopback_hub_create( NULL, ACK_PHASE_MAX_CLIENTS, NULL );
    check( world->hub != NULL );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = ACK_PHASE_PROTOCOL;
    server_config.max_clients = ACK_PHASE_MAX_CLIENTS;
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = world->hub;

    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    world->num_clients = num_clients;
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "loopback";
        client_config.loopback_hub = world->hub;
        client_config.step_driven = true;
        client_config.time_callback = ack_phase_clock;
        world->clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( world->clients[i] != NULL );
    }
}

static void ack_phase_world_destroy( ack_phase_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rn_test_client_stop_and_destroy( world->clients[i] );
    }
    rn_test_server_stop_and_destroy( world->server );
    check( rocketnet_loopback_hub_destroy( world->hub ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_transport_t * ack_phase_transport( rocketnet_server_t * server )
{
    return rocketnet_message_server_transport( rocketnet_server_message_server( server ) );
}

static uint64_t ack_phase_counter( rocketnet_server_t * server, int counter )
{
    const rocketnet_counters_t * counters = rocketnet_server_transport_counters( ack_phase_transport( server ) );
    return counters ? counters->values[counter] : 0;
}

static void ack_phase_connect( ack_phase_world_t * world, int c, double time, uint64_t token_id )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    (void) time;
    check( rn_test_server_token_string( world->backend_secret, world->server, ACK_PHASE_PROTOCOL, addresses, 1,
                                        token_id, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rn_test_client_connect_after_completion( world->clients[c], token ) == ROCKETNET_STATUS_OK );
}

// a message: [kind][client][seq:4] and bytes derived from them
static void ack_phase_write( uint8_t * p, int bytes, int kind, int client, uint32_t seq )
{
    p[0] = (uint8_t) kind;
    p[1] = (uint8_t) client;
    memcpy( p + 2, &seq, 4 );
    for ( int i = 6; i < bytes; i++ )
    {
        p[i] = (uint8_t) ( ( kind * 131 ) ^ ( client * 29 ) ^ (int) ( seq * 7u ) ^ ( i * 13 ) );
    }
}

// the game's side of one frame before the server's update: every client
// updates, sends what this frame's plan says and flushes
static void ack_phase_clients_send( ack_phase_world_t * world, double time, uint64_t plan_seed, uint32_t frame )
{
    g_ack_phase_now = time;
    uint64_t plan = plan_seed;
    for ( int c = 0; c < world->num_clients; c++ )
    {
        rocketnet_client_update( world->clients[c], time );
        const rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( world->clients[c] ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                ack_phase_hash( &world->messages_hash, (uint64_t) c );
                ack_phase_hash( &world->messages_hash, (uint64_t) batch->bytes[m] );
                for ( int b = 0; b < batch->bytes[m]; b++ )
                {
                    ack_phase_hash( &world->messages_hash, batch->data[m][b] );
                }
                world->messages++;
            }
        }
        struct rocketnet_receipt_t receipts[64];
        const int num_receipts = rocketnet_client_receipts( world->clients[c], receipts, 64 );
        for ( int r = 0; r < num_receipts; r++ )
        {
            ack_phase_hash( &world->messages_hash, receipts[r].serial );
            ack_phase_hash( &world->messages_hash, (uint64_t) (int64_t) receipts[r].status );
        }

        if ( rocketnet_client_state( world->clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            // most frames one or two messages; some a burst of big ones, so
            // one client sends many packets in one frame
            const int burst = ack_phase_below( &plan, 16 ) == 0;
            const int count = burst ? 6 + ack_phase_below( &plan, 10 ) : 1 + ack_phase_below( &plan, 2 );
            for ( int m = 0; m < count; m++ )
            {
                const int bytes = burst ? 900 + ack_phase_below( &plan, 200 ) : 16 + ack_phase_below( &plan, 120 );
                const enum rocketnet_reliability_t reliability = ack_phase_below( &plan, 3 ) == 0 ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED
                                                                                                  : ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
                uint8_t * p = rocketnet_client_start_message( world->clients[c], 0, reliability, bytes );
                if ( p )
                {
                    ack_phase_write( p, bytes, 1, c, frame * 64u + (uint32_t) m );
                    (void) rocketnet_client_finish_message( world->clients[c], bytes );
                }
            }
        }
        rocketnet_client_send_packets( world->clients[c] );
    }
}

// the game's side after the server's update: take every message, send to
// every connected slot with receipts, then send
static void ack_phase_server_after( ack_phase_world_t * world, uint64_t plan_seed, uint32_t frame )
{
    uint64_t plan = plan_seed ^ 0x5E5E5E5E5E5E5E5EULL;
    const rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            ack_phase_hash( &world->messages_hash, (uint64_t) batch->client_index[m] );
            ack_phase_hash( &world->messages_hash, (uint64_t) batch->bytes[m] );
            for ( int b = 0; b < batch->bytes[m]; b++ )
            {
                ack_phase_hash( &world->messages_hash, batch->data[m][b] );
            }
            world->messages++;
        }
    }

    for ( int slot = 0; slot < ACK_PHASE_MAX_CLIENTS; slot++ )
    {
        struct rocketnet_receipt_t receipts[256];
        const int num_receipts = rocketnet_server_receipts( world->server, slot, receipts, 256 );
        for ( int r = 0; r < num_receipts; r++ )
        {
            ack_phase_hash( &world->messages_hash, (uint64_t) slot );
            ack_phase_hash( &world->messages_hash, receipts[r].serial );
            ack_phase_hash( &world->messages_hash, (uint64_t) (int64_t) receipts[r].status );
        }

        if ( !rocketnet_server_client_connected( world->server, slot ) )
        {
            continue;
        }
        const int count = 1 + ack_phase_below( &plan, 4 );
        for ( int m = 0; m < count; m++ )
        {
            const int bytes = 16 + ack_phase_below( &plan, 300 );
            const int kind = ack_phase_below( &plan, 3 );
            const enum rocketnet_reliability_t reliability = kind == 0   ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED
                                                             : kind == 1 ? ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT
                                                                         : ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT;
            uint8_t * p = rocketnet_server_start_message( world->server, (uint64_t) slot, 0, reliability, bytes );
            if ( p )
            {
                ack_phase_write( p, bytes, 2, slot, frame * 64u + (uint32_t) m );
                (void) rocketnet_server_finish_message( world->server, bytes );
            }
        }
    }
    rocketnet_server_send_packets( world->server );
}

// the split update with its ack calls dealt to threads: every slot shuffled,
// each dealt to one thread or (one in eight) left for update_end
static void ack_phase_split_update( ack_phase_world_t * world, double time, uint64_t * deal, int num_threads )
{
    rocketnet_server_update_begin( world->server, time );

    int order[ACK_PHASE_MAX_CLIENTS];
    for ( int i = 0; i < ACK_PHASE_MAX_CLIENTS; i++ )
    {
        order[i] = i;
    }
    for ( int i = ACK_PHASE_MAX_CLIENTS - 1; i > 0; i-- )
    {
        const int j = ack_phase_below( deal, i + 1 );
        const int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    std::vector<int> mine[ACK_PHASE_THREADS_MAX];
    for ( int i = 0; i < ACK_PHASE_MAX_CLIENTS; i++ )
    {
        if ( ack_phase_below( deal, 8 ) == 0 )
        {
            continue;
        }
        mine[ack_phase_below( deal, num_threads )].push_back( order[i] );
    }

    int refused[ACK_PHASE_THREADS_MAX] = {};
    std::vector<std::thread> threads;
    for ( int t = 0; t < num_threads; t++ )
    {
        threads.emplace_back( [world, t, &mine, &refused] {
            for ( int slot : mine[t] )
            {
                if ( !rocketnet_server_update_acks( world->server, slot ) )
                {
                    refused[t]++;
                }
            }
        } );
    }
    for ( std::thread & thread : threads )
    {
        thread.join();
    }
    for ( int t = 0; t < num_threads; t++ )
    {
        check( refused[t] == 0 );
    }

    rocketnet_server_update_end( world->server );
}

static bool ack_phase_window_equal( const rn_window_ack_t * a, const rn_window_ack_t * b )
{
    return a->srtt == b->srtt && a->rttvar == b->rttvar && a->resend_delay == b->resend_delay && a->last_ack == b->last_ack &&
           a->last_progress == b->last_progress && a->silence_end == b->silence_end && a->receipt_head == b->receipt_head &&
           a->pkt_head == b->pkt_head && a->pkt_tail == b->pkt_tail && a->error == b->error;
}

// everything the ack apply and the drain write, compared after each update
static int ack_phase_compare( ack_phase_world_t * a, ack_phase_world_t * b )
{
    int differences = 0;
    rocketnet_server_transport_t * ta = ack_phase_transport( a->server );
    rocketnet_server_transport_t * tb = ack_phase_transport( b->server );

    const int * ca = NULL;
    const int * cb = NULL;
    const uint64_t * ia = NULL;
    const uint64_t * ib = NULL;
    const int na = rocketnet_server_transport_acked_packets( ta, &ca, &ia );
    const int nb = rocketnet_server_transport_acked_packets( tb, &cb, &ib );
    const uint64_t * ka = NULL;
    const uint64_t * kb = NULL;
    (void) rocketnet_server_transport_acked_connection_ids( ta, &ka );
    (void) rocketnet_server_transport_acked_connection_ids( tb, &kb );
    if ( na != nb )
    {
        differences++;
    }
    else
    {
        for ( int i = 0; i < na; i++ )
        {
            if ( ca[i] != cb[i] || ia[i] != ib[i] || ( ka && kb && ka[i] != kb[i] ) )
            {
                differences++;
                break;
            }
        }
    }

    const rocketnet_counters_t * xa = rocketnet_server_transport_counters( ta );
    const rocketnet_counters_t * xb = rocketnet_server_transport_counters( tb );
    for ( int k = 0; k < ROCKETNET_NUM_COUNTERS; k++ )
    {
        // the fill-phase counters move only on refusals, which the split
        // world never makes; the rest must agree
        if ( xa->values[k] != xb->values[k] )
        {
            printf( "    transport counter %s: %llu vs %llu\n", rocketnet_counter_name( k ), (unsigned long long) xa->values[k], (unsigned long long) xb->values[k] );
            differences++;
        }
    }

    rn_message_object_t * oa = &rocketnet_server_message_server( a->server )->object;
    rn_message_object_t * ob = &rocketnet_server_message_server( b->server )->object;
    for ( int k = 0; k < ROCKETNET_NUM_COUNTERS; k++ )
    {
        if ( oa->counters.values[k] != ob->counters.values[k] )
        {
            printf( "    message counter %s: %llu vs %llu\n", rocketnet_counter_name( k ), (unsigned long long) oa->counters.values[k], (unsigned long long) ob->counters.values[k] );
            differences++;
        }
    }

    for ( int slot = 0; slot < ACK_PHASE_MAX_CLIENTS; slot++ )
    {
        const rocketnet_ack_endpoint_t * ea = rocketnet_server_transport_ack_endpoint_for_test( ta, slot );
        const rocketnet_ack_endpoint_t * eb = rocketnet_server_transport_ack_endpoint_for_test( tb, slot );
        if ( !ea || !eb || !rocketnet_ack_endpoint_state_equal( ea, eb ) )
        {
            printf( "    ack endpoint %d differs\n", slot );
            differences++;
        }
        if ( oa->hot_connections[slot].state != ob->hot_connections[slot].state ||
             oa->hot_connections[slot].connection_id != ob->hot_connections[slot].connection_id )
        {
            differences++;
        }
        if ( oa->hot_connections[slot].state == RN_MESSAGE_CONNECTION_CONNECTED &&
             !ack_phase_window_equal( &oa->connections[slot].ordered.ack, &ob->connections[slot].ordered.ack ) )
        {
            printf( "    message window %d differs\n", slot );
            differences++;
        }
        if ( oa->hot_connections[slot].acks_taken || ob->hot_connections[slot].acks_taken )
        {
            differences++;                  // the back clears every mark
        }
    }
    return differences;
}

struct ack_phase_run_t
{
    uint64_t seed;
    int num_clients;
    int num_threads;
    int frames;
    int entry_capacity;                     // the spill seam, 0: none
    int loss_percent;
    int latency_ms;
    int jitter_ms;
    int duplicate_percent;
};

struct ack_phase_tally_t
{
    uint64_t acked;
    uint64_t resets;
};

static void ack_phase_differential( const ack_phase_run_t * run, ack_phase_tally_t * tally )
{
    rocketnet_assert_function( ack_phase_assert_hook );
    g_ack_phase_asserts = 0;

    ack_phase_world_t a;
    ack_phase_world_t b;
    ack_phase_world_create( &a, run->num_clients );
    ack_phase_world_create( &b, run->num_clients );
    rocketnet_server_network_simulator( a.server, run->latency_ms, run->jitter_ms, run->loss_percent, run->duplicate_percent );
    rocketnet_server_network_simulator( b.server, run->latency_ms, run->jitter_ms, run->loss_percent, run->duplicate_percent );

    uint64_t events = run->seed * 0x2545F4914F6CDD1DULL + 1;
    uint64_t deal = run->seed ^ 0xD1B54A32D192ED03ULL;
    uint64_t next_token = 0x7100;
    int down_until[ACK_PHASE_MAX_CLIENTS] = {};

    double time = 1000.0;
    for ( int c = 0; c < run->num_clients; c++ )
    {
        ack_phase_connect( &a, c, time, next_token );
        ack_phase_connect( &b, c, time, next_token );
        next_token++;
    }

    int differences = 0;
    for ( int frame = 0; frame < run->frames && differences == 0; frame++ )
    {
        const uint64_t plan_seed = ack_phase_random( &events );

        // events: a client leaves (its disconnect packets reach the server in
        // the same update as its last acks) or the server kicks one; each
        // comes back some frames later with a new token
        if ( frame > 60 && ack_phase_below( &events, 40 ) == 0 )
        {
            const int c = ack_phase_below( &events, run->num_clients );
            if ( down_until[c] == 0 && rocketnet_client_state( a.clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                if ( ack_phase_below( &events, 2 ) == 0 )
                {
                    rocketnet_client_disconnect( a.clients[c] );
                    rocketnet_client_disconnect( b.clients[c] );
                }
                else
                {
                    const int slot = rocketnet_client_index( a.clients[c] );
                    check( slot == rocketnet_client_index( b.clients[c] ) );
                    rocketnet_server_disconnect_client( a.server, slot );
                    rocketnet_server_disconnect_client( b.server, slot );
                }
                down_until[c] = frame + 20 + ack_phase_below( &events, 30 );
                tally->resets++;
            }
        }
        for ( int c = 0; c < run->num_clients; c++ )
        {
            if ( down_until[c] != 0 && frame >= down_until[c] &&
                 rocketnet_client_state( a.clients[c] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED &&
                 rocketnet_client_state( b.clients[c] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
            {
                ack_phase_connect( &a, c, time, next_token );
                ack_phase_connect( &b, c, time, next_token );
                next_token++;
                down_until[c] = 0;
            }
        }

        ack_phase_clients_send( &a, time, plan_seed, (uint32_t) frame );
        ack_phase_clients_send( &b, time, plan_seed, (uint32_t) frame );

        rocketnet_server_acks_test_entry_capacity = run->entry_capacity;

        rocketnet_server_acks_test_direct = 1;
        rocketnet_server_update( a.server, time );
        rocketnet_server_acks_test_direct = 0;

        ack_phase_split_update( &b, time, &deal, run->num_threads );
        rocketnet_server_acks_test_entry_capacity = 0;

        const int frame_differences = ack_phase_compare( &a, &b );
        if ( frame_differences != 0 )
        {
            printf( "    seed %llu frame %d: %d differences\n", (unsigned long long) run->seed, frame, frame_differences );
        }
        differences += frame_differences;

        const int * ci = NULL;
        const uint64_t * ids = NULL;
        tally->acked += (uint64_t) rocketnet_server_transport_acked_packets( ack_phase_transport( b.server ), &ci, &ids );

        ack_phase_server_after( &a, plan_seed, (uint32_t) frame );
        ack_phase_server_after( &b, plan_seed, (uint32_t) frame );
        check( a.messages_hash == b.messages_hash );
        check( a.messages == b.messages );

        time += 1.0 / 60.0;
    }
    check( differences == 0 );
    check( a.messages > 0 );
    check( ack_phase_counter( b.server, ROCKETNET_COUNTER_FILL_PHASE_OVERLAP ) == 0 );
    check( ack_phase_counter( b.server, ROCKETNET_COUNTER_FILL_TWO_FILLERS ) == 0 );
    check( g_ack_phase_asserts == 0 );

    ack_phase_world_destroy( &a );
    ack_phase_world_destroy( &b );
    rocketnet_assert_function( NULL );
}

static void test_server_ack_phase_split_matches_serial()
{
    const ack_phase_run_t runs[] = {
        { 1, 12, 4, 600, 0, 10, 30, 20, 5 },
        { 2, 12, 2, 600, 0, 25, 60, 50, 0 },
        { 3, 16, 6, 500, 0, 5, 20, 40, 10 },
        { 4, 12, 4, 500, 40, 10, 30, 20, 5 },
        { 5, 16, 8, 400, 24, 15, 10, 60, 5 },
        { 6, 8, 3, 600, 0, 0, 0, 0, 0 },
    };
    ack_phase_tally_t tally;
    memset( &tally, 0, sizeof( tally ) );
    for ( const ack_phase_run_t & run : runs )
    {
        ack_phase_differential( &run, &tally );
    }
    printf( "    acked ids compared: %llu, resets: %llu\n", (unsigned long long) tally.acked, (unsigned long long) tally.resets );
    check( tally.acked > 10000 );
    check( tally.resets > 0 );
}

// ---------------------------------------------------------------------------
// the refusals
// ---------------------------------------------------------------------------

static void ack_phase_pump( ack_phase_world_t * world, double * time )
{
    ack_phase_clients_send( world, *time, 7, 0 );
    rocketnet_server_update( world->server, *time );
    ack_phase_server_after( world, 7, 0 );
    *time += 1.0 / 60.0;
}

static void test_server_ack_phase_refusals()
{
    rocketnet_assert_function( ack_phase_assert_hook );
    g_ack_phase_asserts = 0;

    ack_phase_world_t world;
    ack_phase_world_create( &world, 2 );
    double time = 1000.0;
    ack_phase_connect( &world, 0, time, 0x7200 );
    ack_phase_connect( &world, 1, time, 0x7201 );
    for ( int i = 0; i < 300; i++ )
    {
        ack_phase_pump( &world, &time );
    }
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );
    check( rocketnet_server_client_connected( world.server, slot0 ) );
    check( rocketnet_server_client_connected( world.server, slot1 ) );

    const uint64_t overlap = ack_phase_counter( world.server, ROCKETNET_COUNTER_FILL_PHASE_OVERLAP );
    const uint64_t two = ack_phase_counter( world.server, ROCKETNET_COUNTER_FILL_TWO_FILLERS );
    int expected = 0;

    // outside an update: an ack call and update_end are refused

    check( !rocketnet_server_update_acks( world.server, slot0 ) );
    rocketnet_server_update_end( world.server );
    expected += 2;
    check( ack_phase_counter( world.server, ROCKETNET_COUNTER_FILL_PHASE_OVERLAP ) == overlap + 2 );
    check( g_ack_phase_asserts == ack_phase_expected_asserts( expected ) );

    // inside an update: a fill call, another main-thread operation and the
    // one-call update are refused; ack calls and update_end are taken

    ack_phase_clients_send( &world, time, 7, 0 );
    rocketnet_server_update_begin( world.server, time );
    check( rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 ) == NULL );
    rocketnet_server_send_packets( world.server );
    rocketnet_server_update( world.server, time );
    rocketnet_server_update_begin( world.server, time );
    expected += 4;
    check( ack_phase_counter( world.server, ROCKETNET_COUNTER_FILL_PHASE_OVERLAP ) == overlap + 6 );
    check( g_ack_phase_asserts == ack_phase_expected_asserts( expected ) );

    // two threads on one client in one ack phase: the second is refused

    check( rocketnet_server_update_acks( world.server, slot0 ) );
    {
        bool second = true;
        bool other = false;
        std::thread tb( [&] {
            second = rocketnet_server_update_acks( world.server, slot0 );
            other = rocketnet_server_update_acks( world.server, slot1 );
        } );
        tb.join();
        check( !second );
        check( other );
    }
    expected += 1;
    check( ack_phase_counter( world.server, ROCKETNET_COUNTER_FILL_TWO_FILLERS ) == two + 1 );
    check( rocketnet_server_update_acks( world.server, slot0 ) );         // the same thread again: nothing more to apply
    check( !rocketnet_server_update_acks( world.server, -1 ) );
    check( !rocketnet_server_update_acks( world.server, ACK_PHASE_MAX_CLIENTS ) );
    rocketnet_server_update_end( world.server );
    check( g_ack_phase_asserts == ack_phase_expected_asserts( expected ) );

    // after update_end the fill phase takes fill calls again, and the
    // connections live on

    uint8_t * p = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    if ( p )
    {
        memset( p, 0x41, 8 );
        check( rocketnet_server_finish_message( world.server, 8 ) != 0 );
    }
    ack_phase_server_after( &world, 7, 0 );
    time += 1.0 / 60.0;
    for ( int i = 0; i < 60; i++ )
    {
        ack_phase_pump( &world, &time );
    }
    check( rocketnet_server_client_connected( world.server, slot0 ) );
    check( rocketnet_server_client_connected( world.server, slot1 ) );
    check( ack_phase_counter( world.server, ROCKETNET_COUNTER_FILL_PHASE_OVERLAP ) == overlap + 6 );
    check( g_ack_phase_asserts == ack_phase_expected_asserts( expected ) );

    ack_phase_world_destroy( &world );
    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_server_ack_phase_split_matches_serial );
REGISTER_UNIT_TEST( test_server_ack_phase_refusals );

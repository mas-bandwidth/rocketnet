/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A client start_packet can refuse after spending its sequence
// (sdk/rocketnet_client_transport.cpp:1532, :1549, :1570 and :1587). The
// server assigns a sequence only when finish accepts the payload and reserves
// every wire frame, so a server lease, ring or arena refusal, an abort, or a
// refused finish leaves no sequence gap.
// tla/FragmentsAndAcks.tla models such a sequence as a burn (BurnOne,
// :300-310; "a burn spends sequences nothing is ever sent on", :19-20) and
// checks BurnedNeverNamed (:705-707): a burned sequence is never opened and
// never named by an ack block.
//
// The first two cases drive the ack endpoint and the block builder directly.
// The rest cause real refusals on the in-memory loopback transport, time from
// 1000.0, no socket, on the server and on the client, and compare each run
// with a control that sends the same real packets and makes no refused call:
// the ack blocks (every real packet acked, no unknown sequence), the loss
// counters and stats, rtt, the replay window (every later packet delivered,
// none refused), and on the message layer the reliable channel's resends and
// the resend delay estimate. The lease table is not driven here: on the loopback transport
// a fill phase holds at most the payload ring's 256 frames plus
// ROCKETNET_SEND_ARENA_SLOTS arena frames, below its refusal point (half its
// capacity, sdk/rocketnet_packet_lease.h:37). The server assigns the sequence
// after lease insertion, so that refusal also leaves no gap.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "portable_crt.h"

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"
#include "rocketnet_replay.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_layers.h"
#include "rocketnet_message_object.h"
#include "rocketnet_channel.h"
#include "rocketnet_window_ack.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern thread_local unsigned long long g_ack_visit_count;

static void rec_sent( struct rocketnet_ack_endpoint_t * ep, uint64_t sequence )
{
    enum rocketnet_ack_sent_result_t r = rocketnet_ack_endpoint_record_sent_classified(
        ep, sequence, 100, 1000000 + sequence, 1, 0, 1000.0 + (double) sequence * 0.001 );
    rocketnet_unit_check( r != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static void check_bits_match_ring( struct rocketnet_ack_endpoint_t * ep )
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t window_low = 1;
    if ( ep->highest_sequence >= N )
        window_low = ep->highest_sequence - N + 1;
    for ( uint64_t i = 0; i < N; i++ )
    {
        uint64_t held_bit = ( ep->sent_held[i >> 6] >> ( i & 63 ) ) & 1;
        uint64_t unacked_bit = ( ep->sent_unacked[i >> 6] >> ( i & 63 ) ) & 1;
        // a slot not written since the reset is the empty record (sequence 0)
        const bool written = ( ep->sent_written[i >> 6] >> ( i & 63 ) ) & 1;
        int expect_held = written && ep->sent[i].sequence != 0 && ep->sent[i].sequence >= window_low;
        int expect_unacked = expect_held && !ep->sent[i].acked;
        rocketnet_unit_check( ( held_bit != 0 ) == ( expect_held != 0 ) );
        rocketnet_unit_check( ( unacked_bit != 0 ) == ( expect_unacked != 0 ) );
    }
}

// test_sequence_burn_sender_walk_is_harmless: a refused start burns sequence 6
// (spent, never recorded). A block naming the opened span around it acks
// exactly the real records, the walk visits only them, no sequence is
// unknown (none is counted lost), and the shadow bitmaps equal the ring.
// A second identical block visits nothing: the burn resends nothing.

static void test_sequence_burn_sender_walk_is_harmless()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    uint64_t * acked = (uint64_t *) rocketnet_malloc( NULL, N * sizeof( uint64_t ) );
    rocketnet_unit_check( acked );

    for ( uint64_t s = 1; s <= 5; s++ )
        rec_sent( ep, s );

    // burn sequence 6: start_packet spent it, the frame/lease/ring refusal
    // returned before record_sent, so no record exists
    for ( uint64_t s = 7; s <= 12; s++ )
        rec_sent( ep, s );

    // the receiver opened 1..5 and 7..12 (6 never arrived), so its block has
    // a one-sequence gap at 6: a run 7..12, then a run 1..5
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = 12;
    block.first_range_length = 6;
    block.num_ranges = 1;
    block.range_gap[0] = 1;
    block.range_length[0] = 5;

    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;

    g_ack_visit_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, (int) N, &acked_count, &unknown, &batch_full ) == 11 );
    rocketnet_unit_check( acked_count == 11 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( g_ack_visit_count == 11 );
    check_bits_match_ring( ep );

    // a second identical block visits nothing: nothing is resent or re-walked
    g_ack_visit_count = 0;
    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    rocketnet_ack_endpoint_process_block( ep, &block, 3100.0, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 0 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( g_ack_visit_count == 0 );
    check_bits_match_ring( ep );

    rocketnet_free( NULL, acked );
    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( test_sequence_burn_sender_walk_is_harmless );

// the ack block a decoder expands

static int seq_claimed( const struct rocketnet_ack_block_t * b, uint64_t seq )
{
    if ( seq > b->largest )
        return 0;
    {
        uint64_t lo = b->largest - (uint64_t) b->first_range_length + 1;
        if ( seq >= lo )
            return 1;
    }
    uint64_t cur = b->largest - (uint64_t) b->first_range_length + 1;
    for ( int i = 0; i < (int) b->num_ranges; i++ )
    {
        uint64_t high = cur - 1 - (uint64_t) b->range_gap[i];
        uint64_t lo = high - (uint64_t) b->range_length[i] + 1;
        if ( seq >= lo && seq <= high )
            return 1;
        cur = lo;
    }
    return 0;
}

// test_sequence_burn_receiver_block_skips_unopened: the receiver opened 1..3 and
// 5..6; sequence 4 is burned (never sent, never opened). The ack block it
// builds names only what it opened, so the burned sequence never appears in a
// block and never skews the sender's walk.

static void test_sequence_burn_receiver_block_skips_unopened()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );
    for ( uint64_t s = 1; s <= 3; s++ )
        rocketnet_replay_advance( &replay, s );
    // burned sequence 4: start_packet spent it, the packet never arrived
    for ( uint64_t s = 5; s <= 6; s++ )
        rocketnet_replay_advance( &replay, s );

    uint8_t wire[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int bytes = rocketnet_ack_block_build( wire, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay );
    rocketnet_unit_check( bytes > 0 );

    struct rocketnet_ack_block_t block;
    const int parsed = rocketnet_ack_block_read( &block, wire, bytes, 6 );
    rocketnet_unit_check( parsed == bytes );
    rocketnet_unit_check( block.largest == 6 );
    rocketnet_unit_check( block.first_range_length == 2 );
    rocketnet_unit_check( block.num_ranges == 1 );
    rocketnet_unit_check( block.range_gap[0] == 1 );
    rocketnet_unit_check( block.range_length[0] == 3 );

    rocketnet_unit_check( seq_claimed( &block, 1 ) );
    rocketnet_unit_check( seq_claimed( &block, 2 ) );
    rocketnet_unit_check( seq_claimed( &block, 3 ) );
    rocketnet_unit_check( !seq_claimed( &block, 4 ) );
    rocketnet_unit_check( seq_claimed( &block, 5 ) );
    rocketnet_unit_check( seq_claimed( &block, 6 ) );
    rocketnet_unit_check( !seq_claimed( &block, 7 ) );
}
REGISTER_UNIT_TEST( test_sequence_burn_receiver_block_skips_unopened );

// ---- real refused starts over the loopback transports --------------------
//
// A transport pair on the in-memory loopback transport, stepped from 1000.0 by
// 10 ms, no socket. The burning side fills its own capacity with real packets
// and then calls start_packet until it refuses for real (SEND_RING_FULL or
// SEND_ARENA_FULL counts each refusal). Client refusals spend their sequence;
// the server assigns sequences only at an accepted finish, so its refusals spend
// none. The control run sends the very same real
// packets in the very same rounds and never makes the refused calls. Both
// runs report the measures of sequence_burn_measures_t.

#define BURN_ROUNDS 24
#define BURN_SETTLE_ROUNDS 30
#define BURN_REFUSALS_PER_ROUND 3
#define BURN_SMALL_BYTES 32
#define BURN_ARENA_REQUEST ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 600 )

enum sequence_burn_side_t
{
    BURN_SIDE_SERVER = 0,
    BURN_SIDE_CLIENT = 1
};
enum sequence_burn_kind_t
{
    BURN_KIND_RING = 0,
    BURN_KIND_ARENA = 1
};

static double sequence_burn_clock = 1000.0;
static double sequence_burn_time_callback( void * ) { return sequence_burn_clock; }

struct sequence_burn_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    double time;
};

static bool sequence_burn_pair_setup( struct sequence_burn_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    sequence_burn_clock = pair->time;
    rocketnet_quiet( 1 );

    const uint64_t protocol_id = 0x6275726e73657173ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    pair->server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    rocketnet_unit_check( pair->server != NULL );
    if ( !pair->server )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( pair->server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                                    0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = sequence_burn_time_callback;

    pair->client = rocketnet_client_transport_create_sized( NULL, token, &client_config, sizeof( client_config ), pair->time );
    rocketnet_unit_check( pair->client != NULL );
    return pair->client != NULL;
}

static void sequence_burn_pair_teardown( struct sequence_burn_pair_t * pair )
{
    rn_test_client_transport_stop_and_destroy( pair->client );
    rn_test_server_transport_stop_and_destroy( pair->server );
}

// the measures of one run, read on the burning side (the sender) and on its
// receiving side
struct sequence_burn_measures_t
{
    int real_sent;                  // real packets the sender finished
    int refused;                    // start_packet calls refused after the sequence was spent
    int acked;                      // packet ids the sender's acked batches surfaced
    int delivered;                  // payloads the receiver delivered
    uint64_t refusal_counter;       // SEND_RING_FULL or SEND_ARENA_FULL on the sender
    uint64_t lease_full;            // SEND_LEASE_FULL on the sender
    uint64_t ack_unknown;           // the sender's ack walk met a sequence it holds no record of
    uint64_t ack_batch_full;        // the sender's acked batch deferrals
    uint64_t history_lost;          // SEND_HISTORY_LOST on the sender
    uint64_t receiver_already;      // the receiver's replay window refused a payload as already received
    uint64_t receiver_did_not_open; // the receiver could not open a payload
    uint64_t receiver_delivered;    // PAYLOAD_DELIVERED or CLIENT_PAYLOAD_DELIVERED on the receiver
    float client_rtt;               // client stat rtt, ms
    float server_rtt;               // server per-client stat rtt, ms
    float client_loss;              // client stat packet loss (receive-side gaps in the server's sequence), %
    float server_loss;              // server per-client stat packet loss (sent and never acked), %
    int per_round[BURN_ROUNDS];     // real packets sent in each round
};

static bool sequence_burn_round_burns( int round )
{
    return ( round % 4 ) == 1;
}

static uint8_t * sequence_burn_start( struct sequence_burn_pair_t * pair, int side, int request, struct rocketnet_packet_context_t * context )
{
    if ( side == BURN_SIDE_SERVER )
        return rocketnet_server_transport_start_packet( pair->server, 0, request, context );
    return rocketnet_client_transport_start_packet( pair->client, request, context );
}

static bool sequence_burn_send_one( struct sequence_burn_pair_t * pair, int side, int request, int round, int index )
{
    struct rocketnet_packet_context_t context;
    uint8_t * frame = sequence_burn_start( pair, side, request, &context );
    if ( !frame )
        return false;
    uint8_t * payload = frame + context.payload_index;
    memset( payload, (uint8_t) ( 0xB0 + round ), BURN_SMALL_BYTES );
    payload[0] = (uint8_t) index;
    int wire = 0;
    if ( side == BURN_SIDE_SERVER )
        wire = rocketnet_server_transport_finish_packet( pair->server, &context, BURN_SMALL_BYTES );
    else
        wire = rocketnet_client_transport_finish_packet( pair->client, &context, BURN_SMALL_BYTES );
    rocketnet_unit_check( wire > 0 );
    return true;
}

static const struct rocketnet_counters_t * sequence_burn_counters( struct sequence_burn_pair_t * pair, int side )
{
    if ( side == BURN_SIDE_SERVER )
        return rocketnet_server_transport_counters( pair->server );
    return rocketnet_client_transport_counters( pair->client );
}

// one round: update both ends and read the batches, fill, flush
static void sequence_burn_round( struct sequence_burn_pair_t * pair, int side, int kind, bool burn, int round,
                                 const int * control_counts, struct sequence_burn_measures_t * m )
{
    pair->time += 0.01;
    sequence_burn_clock = pair->time;
    rocketnet_server_transport_update( pair->server, pair->time );
    rocketnet_client_transport_update( pair->client, pair->time );

    if ( side == BURN_SIDE_SERVER )
    {
        const int * client_index = NULL;
        const uint64_t * ids = NULL;
        m->acked += rocketnet_server_transport_acked_packets( pair->server, &client_index, &ids );
        m->delivered += rocketnet_client_transport_num_payloads( pair->client );
    }
    else
    {
        const uint64_t * ids = NULL;
        m->acked += rocketnet_client_transport_acked_packets( pair->client, &ids );
        m->delivered += rocketnet_server_transport_num_payloads( pair->server );
    }

    if ( round >= 0 && round < BURN_ROUNDS )
    {
        const int request = kind == BURN_KIND_ARENA ? BURN_ARENA_REQUEST : BURN_SMALL_BYTES;
        int sent = 0;
        if ( control_counts )
        {
            // the control: exactly the burn run's real packets, no refused call
            for ( int i = 0; i < control_counts[round]; i++ )
            {
                rocketnet_unit_check( sequence_burn_send_one( pair, side, request, round, i ) );
                sent++;
            }
        }
        else if ( burn && sequence_burn_round_burns( round ) )
        {
            // real packets until the capacity refuses, then more refused starts
            while ( sequence_burn_send_one( pair, side, request, round, sent ) )
                sent++;
            m->refused++;
            for ( int i = 1; i < BURN_REFUSALS_PER_ROUND; i++ )
            {
                struct rocketnet_packet_context_t context;
                rocketnet_unit_check( sequence_burn_start( pair, side, request, &context ) == NULL );
                m->refused++;
            }
        }
        else
        {
            for ( int i = 0; i < 2; i++ )
            {
                rocketnet_unit_check( sequence_burn_send_one( pair, side, request, round, i ) );
                sent++;
            }
        }
        m->per_round[round] = sent;
        m->real_sent += sent;
    }

    rocketnet_server_transport_send_packets( pair->server );
    rocketnet_client_transport_send_packets( pair->client );
}

static void sequence_burn_run( int side, int kind, bool burn, const int * control_counts, struct sequence_burn_measures_t * m )
{
    memset( m, 0, sizeof( *m ) );
    struct sequence_burn_pair_t pair;
    rocketnet_unit_check( sequence_burn_pair_setup( &pair ) );

    bool connected = false;
    for ( int i = 0; i < 400 && !connected; i++ )
    {
        connected = rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    rocketnet_server_transport_is_client_connected( pair.server, 0 );
        if ( !connected )
        {
            struct sequence_burn_measures_t ignored;
            memset( &ignored, 0, sizeof( ignored ) );
            sequence_burn_round( &pair, side, kind, false, -1, NULL, &ignored );
        }
    }
    rocketnet_unit_check( connected );

    // One final no-payload frame publishes the stats batch completed after the
    // last settling frame. Every intermediate stats batch is stepped below.
    for ( int round = 0; round <= BURN_ROUNDS + BURN_SETTLE_ROUNDS; round++ )
        sequence_burn_round( &pair, side, kind, burn, round, control_counts, m );

    const struct rocketnet_counters_t * sender = sequence_burn_counters( &pair, side );
    const struct rocketnet_counters_t * receiver = sequence_burn_counters( &pair, 1 - side );
    m->refusal_counter = sender->values[kind == BURN_KIND_ARENA ? ROCKETNET_COUNTER_SEND_ARENA_FULL : ROCKETNET_COUNTER_SEND_RING_FULL];
    m->lease_full = sender->values[ROCKETNET_COUNTER_SEND_LEASE_FULL];
    m->history_lost = sender->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST];
    if ( side == BURN_SIDE_SERVER )
    {
        m->ack_unknown = sender->values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE];
        m->ack_batch_full = sender->values[ROCKETNET_COUNTER_ACKED_BATCH_FULL];
        m->receiver_already = receiver->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED];
        m->receiver_did_not_open = receiver->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN];
        m->receiver_delivered = receiver->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED];
    }
    else
    {
        m->ack_unknown = sender->values[ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE];
        m->ack_batch_full = sender->values[ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL];
        m->receiver_already = receiver->values[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED];
        m->receiver_did_not_open = receiver->values[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN];
        m->receiver_delivered = receiver->values[ROCKETNET_COUNTER_PAYLOAD_DELIVERED];
    }

    struct rocketnet_client_transport_stats_t cs;
    struct rocketnet_server_transport_client_stats_t ss;
    rocketnet_client_transport_stats( pair.client, &cs );
    rocketnet_server_transport_client_stats( pair.server, 0, &ss );
    m->client_rtt = cs.rtt;
    m->server_rtt = ss.rtt;
    m->client_loss = cs.packet_loss;
    m->server_loss = ss.packet_loss;

    sequence_burn_pair_teardown( &pair );
}

static void sequence_burn_print( const char * name, const struct sequence_burn_measures_t * m )
{
    char burn_print[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_SEQUENCE_BURN_PRINT", burn_print, sizeof( burn_print ) ) )
        return;
    printf( "BURN %-22s sent=%d refused=%d acked=%d delivered=%d refusal_ctr=%llu lease_full=%llu ack_unknown=%llu "
            "batch_full=%llu history_lost=%llu rx_already=%llu rx_did_not_open=%llu rx_delivered=%llu "
            "client_rtt=%.3f server_rtt=%.3f client_loss=%.4f server_loss=%.4f\n",
            name, m->real_sent, m->refused, m->acked, m->delivered,
            (unsigned long long) m->refusal_counter, (unsigned long long) m->lease_full, (unsigned long long) m->ack_unknown,
            (unsigned long long) m->ack_batch_full, (unsigned long long) m->history_lost,
            (unsigned long long) m->receiver_already, (unsigned long long) m->receiver_did_not_open,
            (unsigned long long) m->receiver_delivered,
            (double) m->client_rtt, (double) m->server_rtt, (double) m->client_loss, (double) m->server_loss );
}

static void sequence_burn_compare( int side, int kind, const char * name )
{
    struct sequence_burn_measures_t burned;
    struct sequence_burn_measures_t control;
    sequence_burn_run( side, kind, true, NULL, &burned );
    sequence_burn_run( side, kind, false, burned.per_round, &control );

    char label[64];
    snprintf( label, sizeof( label ), "%s.burn", name );
    sequence_burn_print( label, &burned );
    snprintf( label, sizeof( label ), "%s.control", name );
    sequence_burn_print( label, &control );

    // the refusals are real, counted by the transport, and only in the burn run
    rocketnet_unit_check( burned.refused == ( BURN_ROUNDS / 4 ) * BURN_REFUSALS_PER_ROUND );
    rocketnet_unit_check( burned.refusal_counter == (uint64_t) burned.refused );
    rocketnet_unit_check( control.refused == 0 && control.refusal_counter == 0 );
    rocketnet_unit_check( burned.lease_full == 0 && control.lease_full == 0 );
    rocketnet_unit_check( burned.real_sent == control.real_sent );

    const struct sequence_burn_measures_t * runs[2] = { &burned, &control };
    for ( int i = 0; i < 2; i++ )
    {
        const struct sequence_burn_measures_t * m = runs[i];
        // ack blocks: every real packet is acknowledged, the walk meets no
        // sequence without a record, nothing is deferred or lost from history
        rocketnet_unit_check( m->acked == m->real_sent );
        rocketnet_unit_check( m->ack_unknown == 0 );
        rocketnet_unit_check( m->ack_batch_full == 0 );
        rocketnet_unit_check( m->history_lost == 0 );
        // the replay window takes every real packet after the burned ones
        rocketnet_unit_check( m->delivered == m->real_sent );
        rocketnet_unit_check( m->receiver_delivered == (uint64_t) m->real_sent );
        rocketnet_unit_check( m->receiver_already == 0 );
        rocketnet_unit_check( m->receiver_did_not_open == 0 );
        rocketnet_unit_check( m->server_rtt > 0.0f );
    }

    // rtt and the sender's loss (sent and never acked) are the control's
    rocketnet_unit_check( burned.client_rtt == control.client_rtt );
    rocketnet_unit_check( burned.server_rtt == control.server_rtt );
    rocketnet_unit_check( burned.server_loss == control.server_loss );

    // the client's loss stat is receive-side, from gaps in the server's
    // session sequence (ROCKETNET_CLIENT_STAT_PACKET_LOSS, sdk/rocketnet.h;
    // sdk/rocketnet_client_transport.cpp:1170 feeds the tracker). A start the
    // server's ring or arena refuses takes no sequence, so it leaves no gap and
    // the stat is the control's. A client burn leaves the server's stat, which
    // is ack-based, untouched.
    rocketnet_unit_check( control.client_loss == 0.0f );
    rocketnet_unit_check( burned.client_loss == control.client_loss );
}

static void test_sequence_burn_server_ring_full_is_harmless() { sequence_burn_compare( BURN_SIDE_SERVER, BURN_KIND_RING, "server.ring" ); }
REGISTER_UNIT_TEST( test_sequence_burn_server_ring_full_is_harmless );
static void test_sequence_burn_server_arena_full_is_harmless() { sequence_burn_compare( BURN_SIDE_SERVER, BURN_KIND_ARENA, "server.arena" ); }
REGISTER_UNIT_TEST( test_sequence_burn_server_arena_full_is_harmless );
static void test_sequence_burn_client_ring_full_is_harmless() { sequence_burn_compare( BURN_SIDE_CLIENT, BURN_KIND_RING, "client.ring" ); }
REGISTER_UNIT_TEST( test_sequence_burn_client_ring_full_is_harmless );
static void test_sequence_burn_client_arena_full_is_harmless() { sequence_burn_compare( BURN_SIDE_CLIENT, BURN_KIND_ARENA, "client.arena" ); }
REGISTER_UNIT_TEST( test_sequence_burn_client_arena_full_is_harmless );

static void test_sequence_burn_server_finish_populates_sequence()
{
    struct sequence_burn_pair_t pair;
    rocketnet_unit_check( sequence_burn_pair_setup( &pair ) );
    bool connected = false;
    for ( int i = 0; i < 400 && !connected; i++ )
    {
        connected = rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    rocketnet_server_transport_is_client_connected( pair.server, 0 );
        if ( !connected )
        {
            struct sequence_burn_measures_t ignored;
            memset( &ignored, 0, sizeof( ignored ) );
            sequence_burn_round( &pair, BURN_SIDE_SERVER, BURN_KIND_RING, false, -1, NULL, &ignored );
        }
    }
    rocketnet_unit_check( connected );

    // Direct, collapsed arena and fragmented sends all publish the first
    // committed wire sequence when finish consumes the lease.
    const int requests[] = { BURN_SMALL_BYTES, BURN_ARENA_REQUEST, BURN_ARENA_REQUEST };
    const int sizes[] = { BURN_SMALL_BYTES, BURN_SMALL_BYTES, BURN_ARENA_REQUEST };
    uint64_t previous_sequence = 0;
    for ( int i = 0; i < 3; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( pair.server, 0, requests[i], &context );
        rocketnet_unit_check( frame != NULL );
        rocketnet_unit_check( context.sequence == 0 );
        memset( frame + context.payload_index, 0xB0, (size_t) sizes[i] );
        rocketnet_unit_check( rocketnet_server_transport_finish_packet( pair.server, &context, sizes[i] ) > 0 );
        rocketnet_unit_check( context.sequence > previous_sequence );
        previous_sequence = context.sequence;
    }
    sequence_burn_pair_teardown( &pair );
}
REGISTER_UNIT_TEST( test_sequence_burn_server_finish_populates_sequence );

enum sequence_burn_uncommitted_kind_t
{
    BURN_UNCOMMITTED_NONE = 0,
    BURN_UNCOMMITTED_ABORT,
    BURN_UNCOMMITTED_REFUSED_FINISH,
    BURN_UNCOMMITTED_FRAGMENT_RESERVE
};

static float sequence_burn_server_uncommitted_loss( int kind, bool fill_ring = false )
{
    struct sequence_burn_pair_t pair;
    rocketnet_unit_check( sequence_burn_pair_setup( &pair ) );

    bool connected = false;
    for ( int i = 0; i < 400 && !connected; i++ )
    {
        connected = rocketnet_client_transport_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    rocketnet_server_transport_is_client_connected( pair.server, 0 );
        if ( !connected )
        {
            struct sequence_burn_measures_t ignored;
            memset( &ignored, 0, sizeof( ignored ) );
            sequence_burn_round( &pair, BURN_SIDE_SERVER, BURN_KIND_RING, false, -1, NULL, &ignored );
        }
    }
    rocketnet_unit_check( connected );

    if ( fill_ring )
    {
        for ( int filled = 0; filled < 256; filled++ )
            rocketnet_unit_check( sequence_burn_send_one( &pair, BURN_SIDE_SERVER, BURN_SMALL_BYTES, 0, filled ) );
    }

    if ( kind != BURN_UNCOMMITTED_NONE )
    {
        const int request = kind == BURN_UNCOMMITTED_FRAGMENT_RESERVE ? BURN_ARENA_REQUEST : BURN_SMALL_BYTES;
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( pair.server, 0, request, &context );
        rocketnet_unit_check( frame != NULL );
        if ( frame )
        {
            if ( kind == BURN_UNCOMMITTED_ABORT )
            {
                rocketnet_server_transport_abort_packet( pair.server, &context );
            }
            else if ( kind == BURN_UNCOMMITTED_FRAGMENT_RESERVE )
            {
                memset( frame + context.payload_index, 0xB0, BURN_ARENA_REQUEST );
                rocketnet_unit_check( rocketnet_server_transport_finish_packet( pair.server, &context, BURN_ARENA_REQUEST ) == 0 );
            }
            else
            {
                memset( frame + context.payload_index, 0xB0, BURN_SMALL_BYTES );
                rocketnet_unit_check( rocketnet_server_transport_finish_packet( pair.server, &context, 0 ) == 0 );
            }
        }
    }

    rocketnet_server_transport_send_packets( pair.server );
    rocketnet_client_transport_send_packets( pair.client );

    struct sequence_burn_measures_t measures;
    memset( &measures, 0, sizeof( measures ) );
    for ( int round = 0; round < BURN_ROUNDS + BURN_SETTLE_ROUNDS; round++ )
        sequence_burn_round( &pair, BURN_SIDE_SERVER, BURN_KIND_RING, false, round, NULL, &measures );

    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( pair.client, &client_stats );
    sequence_burn_pair_teardown( &pair );
    return client_stats.packet_loss;
}

static void test_sequence_burn_server_abort_does_not_count_as_loss()
{
    const float control = sequence_burn_server_uncommitted_loss( BURN_UNCOMMITTED_NONE );
    const float burned = sequence_burn_server_uncommitted_loss( BURN_UNCOMMITTED_ABORT );
    rocketnet_unit_check( control == 0.0f );
    rocketnet_unit_check( burned == control );
}
REGISTER_UNIT_TEST( test_sequence_burn_server_abort_does_not_count_as_loss );

static void test_sequence_burn_server_refused_finish_does_not_count_as_loss()
{
    const float control = sequence_burn_server_uncommitted_loss( BURN_UNCOMMITTED_NONE );
    const float burned = sequence_burn_server_uncommitted_loss( BURN_UNCOMMITTED_REFUSED_FINISH );
    rocketnet_unit_check( control == 0.0f );
    rocketnet_unit_check( burned == control );
}
REGISTER_UNIT_TEST( test_sequence_burn_server_refused_finish_does_not_count_as_loss );

static void test_sequence_burn_server_unreserved_fragment_finish_does_not_count_as_loss()
{
    const float control = sequence_burn_server_uncommitted_loss( BURN_UNCOMMITTED_NONE, true );
    const float burned = sequence_burn_server_uncommitted_loss( BURN_UNCOMMITTED_FRAGMENT_RESERVE, true );
    rocketnet_unit_check( control == 0.0f );
    rocketnet_unit_check( burned == control );
}
REGISTER_UNIT_TEST( test_sequence_burn_server_unreserved_fragment_finish_does_not_count_as_loss );

// ---- the reliable channel and its resend delay over real refused starts -------------
//
// The message layer on the same loopback pair, default message config. Each
// round the burning side sends one 8-byte reliable ordered message, and every
// fourth round a 3,000-byte reliable ordered message (fragmented into
// ROCKETNET_MAX_PACKET_PAYLOAD_BYTES packets by the message layer). In the burn run,
// in each of those fourth rounds the first start_packet the message layer
// makes finds the transport's payload ring full: the test's seam (the
// object's link.start_packet, the one transport call the layer starts frames
// through, sdk/rocketnet_message_object.h:65) first claims every free ring
// slot with a start it aborts, then makes the layer's own call, which the
// transport refuses for real (SEND_RING_FULL). Client starts spend their
// sequence before this refusal; server starts spend none until finish.
// The layer's write pass then treats the frame as not started
// (rn_endpoint_packet_failed, sdk/rocketnet_endpoint.cpp:1100) and writes the
// rest next update. The control run is the same traffic with no seam. Measured
// on the burning side: reliable records resent on the resend delay (MESSAGE_RESENT),
// the ordered window's srtt, rttvar and resend_delay; on the receiving side: every reliable
// message received, in order, and no duplicate.

#define BURN_MESSAGE_ROUNDS 40
#define BURN_MESSAGE_SETTLE 60
#define BURN_MESSAGE_LARGE_BYTES 3000

struct sequence_burn_message_pair_t
{
    struct rocketnet_message_server_t * server;
    struct rocketnet_message_client_t * client;
    double time;
};

struct sequence_burn_message_measures_t
{
    int reliable_sent;
    int reliable_received;
    int large_sent;
    int large_received;
    int reliable_in_order;          // 1 while every received small reliable message carries the next index
    int layer_refused;              // the layer's own start_packet calls the transport refused
    uint64_t ring_full;             // SEND_RING_FULL on the sender's transport
    uint64_t resent;                // MESSAGE_RESENT on the sender
    uint64_t duplicate;             // MESSAGE_DUPLICATE on the receiver
    double srtt;
    double rttvar;
    double resend_delay;
};

// the seam: armed for one update, it fills the ring and lets the layer's own
// start be refused by the transport
static int sequence_burn_seam_side;
static int sequence_burn_seam_armed;
static int sequence_burn_seam_refused;
static uint8_t * ( *sequence_burn_seam_next )( void * transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context );

static uint8_t * sequence_burn_seam_start( void * transport, int client_index, int request_bytes, struct rocketnet_packet_context_t * context )
{
    if ( sequence_burn_seam_armed )
    {
        sequence_burn_seam_armed = 0;
        for ( ;; )
        {
            struct rocketnet_packet_context_t fill;
            uint8_t * frame = sequence_burn_seam_side == BURN_SIDE_SERVER ? rocketnet_server_transport_start_packet( (struct rocketnet_server_transport_t *) transport, client_index, BURN_SMALL_BYTES, &fill ) : rocketnet_client_transport_start_packet( (struct rocketnet_client_transport_t *) transport, BURN_SMALL_BYTES, &fill );
            if ( !frame )
                break;
            if ( sequence_burn_seam_side == BURN_SIDE_SERVER )
                rocketnet_server_transport_abort_packet( (struct rocketnet_server_transport_t *) transport, &fill );
            else
                rocketnet_client_transport_abort_packet( (struct rocketnet_client_transport_t *) transport, &fill );
        }
        uint8_t * frame = sequence_burn_seam_next( transport, client_index, request_bytes, context );
        rocketnet_unit_check( frame == NULL );     // the transport refused the layer's own start
        sequence_burn_seam_refused++;
        return frame;
    }
    return sequence_burn_seam_next( transport, client_index, request_bytes, context );
}

static bool sequence_burn_message_pair_setup( struct sequence_burn_message_pair_t * pair )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    sequence_burn_clock = pair->time;
    rocketnet_quiet( 1 );

    const uint64_t protocol_id = 0x6275726e6d736773ULL;
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_unit_check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_config.server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_config.server_secret_key ) );
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    pair->server = rocketnet_message_server_create( NULL, "127.0.0.1:0", &server_config, NULL, NULL, 0, 0 );
    rocketnet_unit_check( pair->server != NULL );
    if ( !pair->server )
        return false;

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d",
              rocketnet_server_transport_port( rocketnet_message_server_transport( pair->server ) ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { server_address };
    rocketnet_unit_check( rocketnet_write_production_connect_token( backend_secret_key, server_public_key, protocol_id, addresses, 1,
                                                                    0x9e3779b97f4a7c15ULL, 0, 1ULL << 40, 60, NULL,
                                                                    token, sizeof( token ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.step_driven = 1;
    client_config.time_callback = sequence_burn_time_callback;

    pair->client = rocketnet_message_client_create( NULL, token, &client_config, NULL, NULL, 0, 0 );
    rocketnet_unit_check( pair->client != NULL );
    return pair->client != NULL;
}

static void sequence_burn_message_pair_teardown( struct sequence_burn_message_pair_t * pair )
{
    rn_test_message_client_stop_and_destroy( pair->client );
    rn_test_message_server_stop_and_destroy( pair->server );
}

static uint32_t sequence_burn_message_send( struct sequence_burn_message_pair_t * pair, int side, const uint8_t * data, int bytes )
{
    if ( side == BURN_SIDE_SERVER )
        return rocketnet_message_server_send( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, bytes );
    return rocketnet_message_client_send( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, bytes );
}

static void sequence_burn_message_step( struct sequence_burn_message_pair_t * pair, int side, int round, bool burn,
                                        struct sequence_burn_message_measures_t * m )
{
    pair->time += 0.01;
    sequence_burn_clock = pair->time;
    rocketnet_unit_check( rocketnet_message_server_update( pair->server, pair->time ) == ROCKETNET_UPDATE_OK );
    rocketnet_unit_check( rocketnet_message_client_update( pair->client, pair->time ) == ROCKETNET_UPDATE_OK );

    if ( m )
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        for ( ;; )
        {
            const int got = side == BURN_SIDE_SERVER ? rocketnet_message_client_receive( pair->client, NULL, &data, &bytes )
                                                     : rocketnet_message_server_receive( pair->server, NULL, NULL, &data, &bytes );
            if ( !got )
                break;
            if ( bytes == BURN_MESSAGE_LARGE_BYTES && data[0] == 0xE2 )
                m->large_received++;
            if ( bytes == 8 && data[0] == 0xE1 )
            {
                uint32_t index = 0;
                memcpy( &index, data + 4, sizeof( index ) );
                if ( (int) index != m->reliable_received )
                    m->reliable_in_order = 0;
                m->reliable_received++;
            }
            if ( side == BURN_SIDE_SERVER )
                rocketnet_message_client_release( pair->client, data );
            else
                rocketnet_message_server_release( pair->server, data );
        }
    }

    if ( m && round >= 0 && round < BURN_MESSAGE_ROUNDS )
    {
        uint8_t reliable[8] = { 0xE1, 0, 0, 0, 0, 0, 0, 0 };
        const uint32_t index = (uint32_t) m->reliable_sent;
        memcpy( reliable + 4, &index, sizeof( index ) );
        rocketnet_unit_check( sequence_burn_message_send( pair, side, reliable, sizeof( reliable ) ) != 0 );
        m->reliable_sent++;

        if ( sequence_burn_round_burns( round ) )
        {
            static uint8_t large[BURN_MESSAGE_LARGE_BYTES];
            memset( large, 0x5A, sizeof( large ) );
            large[0] = 0xE2;
            rocketnet_unit_check( sequence_burn_message_send( pair, side, large, sizeof( large ) ) != 0 );
            m->large_sent++;
            sequence_burn_seam_armed = burn ? 1 : 0;
        }
    }

    rocketnet_message_server_send_packets( pair->server );
    rocketnet_message_client_send_packets( pair->client );
    rocketnet_unit_check( sequence_burn_seam_armed == 0 );
}

static void sequence_burn_message_run( int side, bool burn, struct sequence_burn_message_measures_t * m )
{
    memset( m, 0, sizeof( *m ) );
    m->reliable_in_order = 1;
    struct sequence_burn_message_pair_t pair;
    rocketnet_unit_check( sequence_burn_message_pair_setup( &pair ) );

    for ( int i = 0; i < 400 && !( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                                   rocketnet_message_server_client_connected( pair.server, 0 ) );
          i++ )
        sequence_burn_message_step( &pair, side, -1, false, NULL );
    rocketnet_unit_check( rocketnet_message_server_client_connected( pair.server, 0 ) );
    rocketnet_unit_check( rocketnet_message_client_state( pair.client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    struct rn_message_object_t * sender = side == BURN_SIDE_SERVER ? &pair.server->object : &pair.client->object;
    struct rn_message_object_t * receiver = side == BURN_SIDE_SERVER ? &pair.client->object : &pair.server->object;
    sequence_burn_seam_side = side;
    sequence_burn_seam_armed = 0;
    sequence_burn_seam_refused = 0;
    sequence_burn_seam_next = sender->link.start_packet;
    if ( burn )
        sender->link.start_packet = sequence_burn_seam_start;

    for ( int round = 0; round < BURN_MESSAGE_ROUNDS + BURN_MESSAGE_SETTLE; round++ )
        sequence_burn_message_step( &pair, side, round, burn, m );

    sender->link.start_packet = sequence_burn_seam_next;

    const struct rocketnet_counters_t * transport = side == BURN_SIDE_SERVER ? rocketnet_server_transport_counters( rocketnet_message_server_transport( pair.server ) ) : rocketnet_client_transport_counters( rocketnet_message_client_transport( pair.client ) );
    m->layer_refused = sequence_burn_seam_refused;
    m->ring_full = transport->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    m->resent = sender->counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];
    m->duplicate = receiver->counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE];
    const struct rn_window_ack_t * ack = &sender->connections[0].ordered.ack;
    m->srtt = ack->srtt;
    m->rttvar = ack->rttvar;
    m->resend_delay = ack->resend_delay;

    sequence_burn_message_pair_teardown( &pair );
}

static void sequence_burn_message_print( const char * name, const struct sequence_burn_message_measures_t * m )
{
    char burn_print[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_SEQUENCE_BURN_PRINT", burn_print, sizeof( burn_print ) ) )
        return;
    printf( "BURN %-22s reliable_sent=%d reliable_received=%d large_sent=%d large_received=%d in_order=%d layer_refused=%d "
            "ring_full=%llu resent=%llu duplicate=%llu srtt=%.6f rttvar=%.6f resend_delay=%.6f\n",
            name, m->reliable_sent, m->reliable_received, m->large_sent, m->large_received, m->reliable_in_order,
            m->layer_refused, (unsigned long long) m->ring_full, (unsigned long long) m->resent,
            (unsigned long long) m->duplicate, m->srtt, m->rttvar, m->resend_delay );
}

static void sequence_burn_message_compare( int side, const char * name )
{
    struct sequence_burn_message_measures_t burned;
    struct sequence_burn_message_measures_t control;
    sequence_burn_message_run( side, true, &burned );
    sequence_burn_message_run( side, false, &control );

    char label[64];
    snprintf( label, sizeof( label ), "%s.burn", name );
    sequence_burn_message_print( label, &burned );
    snprintf( label, sizeof( label ), "%s.control", name );
    sequence_burn_message_print( label, &control );

    // the layer's own starts were refused by the transport in the burn run only
    rocketnet_unit_check( burned.layer_refused == BURN_MESSAGE_ROUNDS / 4 );
    rocketnet_unit_check( burned.ring_full >= (uint64_t) burned.layer_refused );
    rocketnet_unit_check( control.layer_refused == 0 && control.ring_full == 0 );

    const struct sequence_burn_message_measures_t * runs[2] = { &burned, &control };
    for ( int i = 0; i < 2; i++ )
    {
        const struct sequence_burn_message_measures_t * m = runs[i];
        rocketnet_unit_check( m->reliable_received == m->reliable_sent );
        rocketnet_unit_check( m->large_received == m->large_sent );
        rocketnet_unit_check( m->reliable_in_order == 1 );
    }

    // no resend and no duplicate beyond the control's
    rocketnet_unit_check( burned.resent <= control.resent );
    rocketnet_unit_check( burned.duplicate <= control.duplicate );

    // the resend delay estimate: a refused start delays one packet by one update
    // (10 ms here), so the samples differ by less than a step; the burned
    // sequences add no sample and no back-off
    rocketnet_unit_check( fabs( burned.srtt - control.srtt ) < 0.01 );
    const double resend_delay_difference_bound = 2.0 * fabs( burned.srtt - control.srtt ) + 4.0 * fabs( burned.rttvar - control.rttvar ) + 1e-9;
    rocketnet_unit_check( fabs( burned.resend_delay - control.resend_delay ) <= resend_delay_difference_bound );
}

static void test_sequence_burn_server_reliable_and_resend_delay_unchanged() { sequence_burn_message_compare( BURN_SIDE_SERVER, "server.message" ); }
REGISTER_UNIT_TEST( test_sequence_burn_server_reliable_and_resend_delay_unchanged );
static void test_sequence_burn_client_reliable_and_resend_delay_unchanged() { sequence_burn_message_compare( BURN_SIDE_CLIENT, "client.message" ); }
REGISTER_UNIT_TEST( test_sequence_burn_client_reliable_and_resend_delay_unchanged );

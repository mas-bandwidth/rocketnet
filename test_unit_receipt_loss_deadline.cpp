/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// UNRELIABLE_WITH_ACK_RECEIPT: a delivered message whose packet ACK arrives
// after the resend rto, and before the receipt deadline, reports ACKED, never
// LOSS (tla/RNReceipts.tla SweepLoss, LossOnlyAfterReceiptDeadline). Two
// endpoints back to back, time a loop variable from 1000.0 in whole
// milliseconds. No socket, no clock, no transport.

#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_allocator.h"
#include "sdk/rocketnet_counters.h"
#include "sdk/rocketnet_endpoint.h"
#include "sdk/rocketnet_message.h"
#include "sdk/rocketnet_send_budget.h"
#include "sdk/rocketnet_window_ack.h"

#define check rocketnet_unit_check

#define RLD_MAX_SENDS 256

struct rld_packet_t
{
    int arrive_ms;
    uint64_t id;
    int bytes;
    uint8_t data[ROCKETNET_MAX_PAYLOAD_BYTES];
};

struct rld_ack_t
{
    int arrive_ms;
    uint64_t id;
};

struct rld_side_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_endpoint_t endpoint;
    uint64_t next_packet_id;
    std::vector<rld_packet_t> inbound;
    std::vector<rld_ack_t> acks;
    int received;                               // the peer's receipt messages delivered here
};

struct rld_sender_t
{
    int sends;
    uint32_t serial[RLD_MAX_SENDS];
    int send_ms[RLD_MAX_SENDS];
    double rto_at_send[RLD_MAX_SENDS];
    int acked[RLD_MAX_SENDS];
    int lost[RLD_MAX_SENDS];
};

static double rld_now( int ms )
{
    return 1000.0 + ms * 0.001;
}

static void rld_side_create( rld_side_t * side )
{
    static rocketnet_message_config_t config;               // the endpoint keeps a pointer to it
    rocketnet_message_default_config( &config );
    memset( &side->counters, 0, sizeof( side->counters ) );
    side->arena = rn_arena_create( NULL, &config, 1, &side->counters );
    check( side->arena != NULL );
    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = &config;
    endpoint_config.send_kbps = 100000;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PAYLOAD_BYTES;
    rn_endpoint_init( &side->endpoint, rn_arena_region( side->arena, 0 ), &endpoint_config );
    side->next_packet_id = 1;
    side->received = 0;
}

static void rld_side_destroy( rld_side_t * side )
{
    rn_arena_destroy( side->arena );
}

// one update of `side`: acks in, sweep, payloads in (each opened payload is
// acked back over the path), the application's send, the write pass out
static void rld_update( rld_side_t * side, rld_side_t * peer, int ms, int one_way_ms, bool send_one, rld_sender_t * sender )
{
    const double now = rld_now( ms );
    rn_endpoint_t * endpoint = &side->endpoint;
    rn_endpoint_begin_update( endpoint );

    for ( size_t k = 0; k < side->acks.size(); )
    {
        if ( side->acks[k].arrive_ms <= ms )
        {
            rn_endpoint_on_acked( endpoint, side->acks[k].id, now );
            side->acks.erase( side->acks.begin() + (long) k );
        }
        else
            k++;
    }

    rn_endpoint_tick( endpoint, now );

    std::vector<rld_packet_t> batch;
    for ( size_t k = 0; k < side->inbound.size(); )
    {
        if ( side->inbound[k].arrive_ms <= ms )
        {
            batch.push_back( side->inbound[k] );
            side->inbound.erase( side->inbound.begin() + (long) k );
        }
        else
            k++;
    }
    for ( size_t k = 0; k < batch.size(); k++ )
    {
        rld_ack_t ack;
        ack.arrive_ms = ms + one_way_ms;
        ack.id = batch[k].id;
        peer->acks.push_back( ack );
        rn_delivery_t out[64];
        int count = -1;
        rn_endpoint_on_payload( endpoint, batch[k].data, batch[k].bytes, out, 64, &count, now );
        check( count >= 0 );
        for ( int i = 0; i < count; i++ )
            if ( out[i].bytes == 4 )
                side->received++;
    }
    rn_endpoint_config_tick( endpoint, 1, now );
    check( rn_endpoint_error( endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );

    if ( sender )
    {
        rocketnet_message_receipt_t receipts[64];
        const int n = rn_endpoint_take_receipts( endpoint, receipts, 64 );
        for ( int i = 0; i < n; i++ )
        {
            for ( int s = 0; s < sender->sends; s++ )
            {
                if ( sender->serial[s] != receipts[i].serial )
                    continue;
                if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                    sender->acked[s]++;
                else
                    sender->lost[s]++;
            }
        }
    }

    if ( send_one && sender && sender->sends < RLD_MAX_SENDS )
    {
        const uint32_t index = (uint32_t) sender->sends;
        const uint32_t serial = rn_endpoint_send( endpoint, (const uint8_t *) &index, 4, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
                                                  ROCKETNET_PRIORITY_MEDIUM, 0, 0 );
        check( serial != 0 );
        sender->serial[index] = serial;
        sender->send_ms[index] = ms;
        sender->rto_at_send[index] = endpoint->ordered.ack.rto;
        sender->sends++;
    }

    for ( ;; )
    {
        const int request = rn_endpoint_next_packet_bytes( endpoint, now );
        if ( request == 0 )
            break;
        rld_packet_t packet;
        packet.id = side->next_packet_id++;
        const int bytes = rn_endpoint_write_packet( endpoint, packet.data, (int) sizeof( packet.data ), packet.id, now );
        if ( bytes == 0 )
            continue;
        packet.bytes = bytes;
        packet.arrive_ms = ms + one_way_ms;
        rn_endpoint_packet_sent( endpoint, packet.id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), now );
        peer->inbound.push_back( packet );
    }
}

// a sends one UNRELIABLE_WITH_ACK_RECEIPT message every send_every_ms until
// `sends`, b updates every peer_every_ms, both over a lossless path of
// one_way_ms each way; then both run on until every receipt has an outcome.
static void rld_run( int one_way_ms, int peer_every_ms, int send_every_ms, int sends, rld_sender_t * sender, rld_side_t * a, rld_side_t * b )
{
    memset( sender, 0, sizeof( *sender ) );
    rld_side_create( a );
    rld_side_create( b );
    rn_endpoint_reset( &a->endpoint, rld_now( 0 ) );
    rn_endpoint_reset( &b->endpoint, rld_now( 0 ) );
    rn_endpoint_announce_config( &a->endpoint, rld_now( 0 ) );
    rn_endpoint_announce_config( &b->endpoint, rld_now( 0 ) );

    const int end_ms = sends * send_every_ms + 4 * one_way_ms + 2 * peer_every_ms + 5000;
    for ( int ms = 0; ms <= end_ms; ms++ )
    {
        const bool send_one = ( ms % send_every_ms ) == 0 && sender->sends < sends;
        rld_update( a, b, ms, one_way_ms, send_one, sender );
        if ( ( ms % peer_every_ms ) == 0 )
            rld_update( b, a, ms, one_way_ms, false, NULL );
    }

    // every send had exactly one outcome
    for ( int s = 0; s < sender->sends; s++ )
        check( sender->acked[s] + sender->lost[s] == 1 );
    check( b->received == sender->sends );                      // the peer received every one of them
}

// the earliest a receipt packet's ack can arrive: the peer opens it at its
// next update at or after arrival and its ack takes one_way_ms back
static int rld_ack_ms( int send_ms, int one_way_ms, int peer_every_ms )
{
    const int arrive = send_ms + one_way_ms;
    const int opened = ( ( arrive + peer_every_ms - 1 ) / peer_every_ms ) * peer_every_ms;
    return opened + one_way_ms;
}

// DONE-WHEN (1): a 300 ms one-way path, both ends updating every millisecond.
// Every ack comes back 600 ms after its send, beyond rto_max (250 ms), and
// before ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS: every receipt is ACKED.
static void receipt_loss_deadline_long_path_reports_acked()
{
    rld_sender_t sender;
    rld_side_t a, b;
    rld_run( 300, 1, 10, 20, &sender, &a, &b );
    for ( int s = 0; s < sender.sends; s++ )
    {
        const int ack_ms = rld_ack_ms( sender.send_ms[s], 300, 1 ) - sender.send_ms[s];
        check( ack_ms * 0.001 > sender.rto_at_send[s] );        // the ack is slower than the resend rto
        check( ack_ms < ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS );    // and faster than the receipt deadline
        check( sender.lost[s] == 0 );
        check( sender.acked[s] == 1 );
    }
    check( a.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    rld_side_destroy( &a );
    rld_side_destroy( &b );
}

// DONE-WHEN (1): a 1 ms path to a peer that updates (so acks) every 100 ms.
// A message sent just after the peer's update waits up to 100 ms to be
// opened; its ack is slower than the rto, and it is ACKED.
static void receipt_loss_deadline_slow_acking_peer_reports_acked()
{
    rld_sender_t sender;
    rld_side_t a, b;
    rld_run( 1, 100, 7, 100, &sender, &a, &b );
    int slower_than_rto = 0;
    for ( int s = 0; s < sender.sends; s++ )
    {
        const int ack_ms = rld_ack_ms( sender.send_ms[s], 1, 100 ) - sender.send_ms[s];
        if ( ack_ms * 0.001 > sender.rto_at_send[s] )
            slower_than_rto++;
        check( ack_ms < ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS );
        check( sender.lost[s] == 0 );
        check( sender.acked[s] == 1 );
    }
    check( slower_than_rto > 0 );                               // the case holds acks slower than the rto
    check( a.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 0 );
    rld_side_destroy( &a );
    rld_side_destroy( &b );
}

struct rld_ack_fixture_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_window_send_t send;
    rn_window_ack_t ack;
};

static void rld_ack_fixture_init( rld_ack_fixture_t * f, int rto_initial_ms, int rto_max_ms )
{
    memset( f, 0, sizeof( *f ) );
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.rto_initial_ms = rto_initial_ms;
    config.rto_max_ms = rto_max_ms;
    config.reliable_timeout_ms = 60000;                         // validation: at least four resends of the rto fit
    f->arena = rn_arena_create( NULL, &config, 1, &f->counters );
    check( f->arena != NULL );
    rn_region_t * region = rn_arena_region( f->arena, 0 );
    rn_window_send_init( &f->send, region, 0 );
    rn_window_ack_init( &f->ack, &f->send, region, &config );
}

static int rld_ack_fixture_outcomes( rld_ack_fixture_t * f, rocketnet_message_receipt_t * out )
{
    rn_window_ack_receipt_end_update( &f->ack );
    const int n = rn_window_ack_receipts( &f->ack, out, 16 );
    rn_window_ack_receipt_begin_update( &f->ack );
    return n;
}

// DONE-WHEN (2): an unacknowledged receipt packet reports LOSS at its send
// time plus ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS, not one millisecond before.
static void receipt_loss_deadline_is_the_receipt_timeout()
{
    rld_ack_fixture_t f;
    rld_ack_fixture_init( &f, 100, 250 );
    rn_window_ack_receipt_begin_update( &f.ack );
    const uint32_t serial = 7;
    const double send = 1000.0;
    const double timeout = ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS / 1000.0;
    rn_window_ack_commit_receipt_packet( &f.ack, 11, &serial, 1, send );
    rocketnet_message_receipt_t out[16];
    rn_window_ack_receipt_sweep( &f.ack, send + f.ack.rto );                // the rto alone is not the deadline
    check( rld_ack_fixture_outcomes( &f, out ) == 0 );
    rn_window_ack_receipt_sweep( &f.ack, send + timeout - 0.001 );
    check( rld_ack_fixture_outcomes( &f, out ) == 0 );
    rn_window_ack_receipt_sweep( &f.ack, send + timeout );
    check( rld_ack_fixture_outcomes( &f, out ) == 1 );
    check( out[0].serial == serial && out[0].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    check( rn_window_ack_on_ack( &f.ack, 11, send + timeout + 0.001 ) == 1 );  // the late ack stutters
    check( rld_ack_fixture_outcomes( &f, out ) == 0 );
    check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == 1 );
    check( f.counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == 0 );
    rn_arena_destroy( f.arena );
}

// DONE-WHEN (2): the deadline is never below the resend rto. With an rto
// above ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS, LOSS waits for the rto, and an ack
// before it is ACKED.
static void receipt_loss_deadline_never_below_the_rto()
{
    const int rto_ms = ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS + 1000;
    rld_ack_fixture_t f;
    rld_ack_fixture_init( &f, rto_ms, rto_ms );
    check( f.ack.rto > ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS / 1000.0 );
    rn_window_ack_receipt_begin_update( &f.ack );
    const uint32_t serials[2] = { 8, 9 };
    const double send = 1000.0;
    rn_window_ack_commit_receipt_packet( &f.ack, 21, &serials[0], 1, send );
    rn_window_ack_commit_receipt_packet( &f.ack, 22, &serials[1], 1, send );
    rocketnet_message_receipt_t out[16];
    rn_window_ack_receipt_sweep( &f.ack, send + ROCKETNET_RECEIPT_LOSS_TIMEOUT_MS / 1000.0 );
    check( rld_ack_fixture_outcomes( &f, out ) == 0 );
    check( rn_window_ack_on_ack( &f.ack, 21, send + f.ack.rto - 0.001 ) == 1 );
    check( rld_ack_fixture_outcomes( &f, out ) == 1 );
    check( out[0].serial == 8 && out[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );
    rn_window_ack_receipt_sweep( &f.ack, send + f.ack.rto );
    check( rld_ack_fixture_outcomes( &f, out ) == 1 );
    check( out[0].serial == 9 && out[0].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    rn_arena_destroy( f.arena );
}

REGISTER_UNIT_TEST( receipt_loss_deadline_long_path_reports_acked );
REGISTER_UNIT_TEST( receipt_loss_deadline_slow_acking_peer_reports_acked );
REGISTER_UNIT_TEST( receipt_loss_deadline_is_the_receipt_timeout );
REGISTER_UNIT_TEST( receipt_loss_deadline_never_below_the_rto );
